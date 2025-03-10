extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
}

#include <iostream>

static AVBufferRef *hw_device_ctx = NULL;// 硬件设备上下文
static enum AVPixelFormat hw_pix_fmt;
static FILE *output_file = NULL; // 解码后 编码前 输出文件

AVCodecContext* codec_ctx_en = nullptr; // 编码器上下文
// 帧率（30帧/秒）
const AVRational frame_rate = { 30, 1 };// 帧率
// 输出文件名
const char* output_filename = "encode_output.mp4"; // 编码后输出文件名
AVFormatContext* fmt_ctx_en = nullptr;  // 输出文件上下文
AVPacket * pkt = nullptr;             // 编码后的数据包
AVStream * stream = nullptr;	// 编码后输出流

#define ENCODE_OPEN 1
int64_t num_frames = 0; // 解码帧数


// 硬件加速初始化
static int hw_decoder_init(AVCodecContext *ctx, const enum AVHWDeviceType type)
{
	int err = 0;
	// 创建一个硬件设备上下文
	if ((err = av_hwdevice_ctx_create(&hw_device_ctx, type,	NULL, NULL, 0)) < 0)
	{
		fprintf(stderr, "Failed to create specified HW device.\n");
		return err;
	}
	ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);

	return err;
}

// 获取GPU硬件解码帧的格式
static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts)
{
	const enum AVPixelFormat *p;

	for (p = pix_fmts; *p != -1; p++)
	{
		if (*p == hw_pix_fmt)
			return *p;
	}

	fprintf(stderr, "Failed to get HW surface format.\n");
	return AV_PIX_FMT_NONE;
}

// 解码后数据格式转换，GPU到CPU拷贝，YUV数据dump到文件
static int decode_write(AVCodecContext *avctx, AVPacket *packet)
{
	AVFrame *frame = NULL, *sw_frame = NULL;
	AVFrame *tmp_frame = NULL;
	uint8_t *buffer = NULL;
	int size;
	int ret = 0;

	ret = avcodec_send_packet(avctx, packet);
	if (ret < 0)
	{
		fprintf(stderr, "Error during decoding\n");
		return ret;
	}

	while (1)
	{
		if (!(frame = av_frame_alloc()) || !(sw_frame = av_frame_alloc()))
		{
			fprintf(stderr, "Can not alloc frame\n");
			ret = AVERROR(ENOMEM);
			goto fail;
		}

		ret = avcodec_receive_frame(avctx, frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
		{
			av_frame_free(&frame);
			av_frame_free(&sw_frame);
			return 0;
		}
		else if (ret < 0)
		{
			fprintf(stderr, "Error while decoding\n");
			goto fail;
		}

		// if (frame->format == hw_pix_fmt)
		// {
		// 	/* 将解码后的数据从GPU内存存格式转为CPU内存格式，并完成GPU到CPU内存的拷贝*/
		// 	if ((ret = av_hwframe_transfer_data(sw_frame, frame, 0)) < 0)
		// 	{
		// 		fprintf(stderr, "Error transferring the data to system memory\n");
		// 		goto fail;
		// 	}
		// 	tmp_frame = sw_frame;
		// }
		// else
		//	tmp_frame = frame;
		tmp_frame = frame;

		
		if(ENCODE_OPEN)
		{
			// 设置帧的显示时间戳（PTS）
			//tmp_frame->pts = num_frames * 3000;
			//num_frames++;

			// 发送帧到编码器
			ret = avcodec_send_frame(codec_ctx_en, tmp_frame);
			if (ret < 0)
			{
				throw std::runtime_error("Error sending frame to encoder");
			}
	
			// 接收编码后的数据包
			pkt = av_packet_alloc();
			while (ret >= 0)
			{				
				ret = avcodec_receive_packet(codec_ctx_en, pkt);
				if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
				{
					av_packet_free(&pkt);
					ret = 0;
					break;
				}	
				
				// 设置数据包的流索引和时间基
				pkt->stream_index = stream->index;
				//pkt->time_base = AVRational{1, 90000};
	
				// 写入数据包到输出文件
				ret = av_interleaved_write_frame(fmt_ctx_en, pkt);
				if (ret < 0)
				{
					throw std::runtime_error("Error writing packet to file");
				}
	
				// 释放数据包
				av_packet_unref(pkt);
			}

		}

		
		

		/*
		// 计算一张YUV图需要的内存 大小
		size = av_image_get_buffer_size((AVPixelFormat)tmp_frame->format, tmp_frame->width,	tmp_frame->height, 1);
		// 分配内存
		buffer = (uint8_t *)av_malloc(size);
		if (!buffer)
		{
			fprintf(stderr, "Can not alloc buffer\n");
			ret = AVERROR(ENOMEM);
			goto fail;
		}
		// 将图片数据拷贝的buffer中(按行拷贝)
		ret = av_image_copy_to_buffer(buffer, size,	(const uint8_t *const *)tmp_frame->data,(const int *)tmp_frame->linesize, 
									(AVPixelFormat)tmp_frame->format, tmp_frame->width, tmp_frame->height, 1);
		if (ret < 0)
		{
			fprintf(stderr, "Can not copy image to buffer\n");
			goto fail;
		}
		// buffer数据dump到文件
		if ((ret = fwrite(buffer, 1, size, output_file)) < 0)
		{
			fprintf(stderr, "Failed to dump raw data.\n");
			goto fail;
		}
        */

	fail:
		av_frame_free(&frame);
		av_frame_free(&sw_frame);
		av_freep(&buffer);
		if (ret < 0)
			return ret;
	}
}
int main(int argc, char *argv[])
{
	AVFormatContext *input_ctx = NULL;
	int video_stream, ret;
	AVStream *video = NULL;
	AVCodecContext *decoder_ctx = NULL;
	const AVCodec * decoder = NULL;
	AVPacket packet;
	enum AVHWDeviceType type;
	int i;

	if (argc < 4)
	{
		fprintf(stderr, "Usage: %s <device type> <input file> <output file>\n", argv[0]);
		return -1;
	}
	// 设备类型为：cuda dxva2 qsv d3d11va opencl，通常在windows使用d3d11va或者dxva2
	type = av_hwdevice_find_type_by_name(argv[1]); // 根据设备名找到设备类型
	if (type == AV_HWDEVICE_TYPE_NONE)
	{
		fprintf(stderr, "Device type %s is not supported.\n", argv[1]);
		fprintf(stderr, "Available device types:");
		while ((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE)
			fprintf(stderr, " %s", av_hwdevice_get_type_name(type));
		fprintf(stderr, "\n");
		return -1;
	}

	/* open the input file */
	if (avformat_open_input(&input_ctx, argv[2], NULL, NULL) != 0)
	{
		fprintf(stderr, "Cannot open input file '%s'\n", argv[2]);
		return -1;
	}

	if (avformat_find_stream_info(input_ctx, NULL) < 0)
	{
		fprintf(stderr, "Cannot find input stream information.\n");
		return -1;
	}

	/* find the video stream information */
	ret = av_find_best_stream(input_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
	if (ret < 0)
	{
		fprintf(stderr, "Cannot find a video stream in the input file\n");
		return -1;
	}
	video_stream = ret;

	// 查找到对应硬件类型解码后的数据格式
	for (i = 0;; i++)
	{
		const AVCodecHWConfig *config = avcodec_get_hw_config(decoder, i);
		if (!config)
		{
			fprintf(stderr, "Decoder %s does not support device type %s.\n",
					decoder->name, av_hwdevice_get_type_name(type));
			return -1;
		}
		if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
			config->device_type == type)
		{
			hw_pix_fmt = config->pix_fmt;
			break;
		}
	}

	if (!(decoder_ctx = avcodec_alloc_context3(decoder)))
		return AVERROR(ENOMEM);

	video = input_ctx->streams[video_stream];
	if (avcodec_parameters_to_context(decoder_ctx, video->codecpar) < 0)
		return -1;

	decoder_ctx->get_format = get_hw_format;

	// 硬件加速初始化
	if (hw_decoder_init(decoder_ctx, type) < 0)
		return -1;

	if ((ret = avcodec_open2(decoder_ctx, decoder, NULL)) < 0)
	{
		fprintf(stderr, "Failed to open codec for stream #%u\n", video_stream);
		return -1;
	}

//****************************************** */

	if(ENCODE_OPEN)
	{

		// 2. 创建硬件帧上下文
		AVBufferRef *hw_frames_ref = av_hwframe_ctx_alloc(hw_device_ctx);
		if (!hw_frames_ref)
		{
			throw std::runtime_error("Failed to create hardware frames context");
		}

		// 配置硬件帧上下文参数
		AVHWFramesContext *hw_frames_ctx = (AVHWFramesContext *)hw_frames_ref->data;
		hw_frames_ctx->format = AV_PIX_FMT_VAAPI;	// 硬件像素格式
		hw_frames_ctx->sw_format = AV_PIX_FMT_NV12; // 软件像素格式
		hw_frames_ctx->width = 3840;				// 视频宽度
		hw_frames_ctx->height = 2160;				// 视频高度
		hw_frames_ctx->initial_pool_size = 20;		// 初始帧池大小

		// 3. 查找编码器（使用 hevc_vaapi 编码器）
		const AVCodec *codec_en = avcodec_find_encoder_by_name("hevc_vaapi");
		if (!codec_en)
		{
			throw std::runtime_error("Codec vaapi not found");
		}

		// 4. 创建编码器上下文
		codec_ctx_en = avcodec_alloc_context3(codec_en);
		if (!codec_ctx_en)
		{
			throw std::runtime_error("Could not allocate video codec context");
		}

		// 配置编码器参数
		codec_ctx_en->hw_frames_ctx = av_buffer_ref(hw_frames_ref); // 绑定硬件帧上下文
		codec_ctx_en->width = 3840;									// 视频宽度
		codec_ctx_en->height = 2160;								// 视频高度
		codec_ctx_en->time_base = av_inv_q(frame_rate);				// 时间基（帧率的倒数）
		codec_ctx_en->framerate = frame_rate;						// 帧率
		codec_ctx_en->pix_fmt = AV_PIX_FMT_VAAPI;					// 像素格式
		codec_ctx_en->bit_rate = 10 * 1024 * 1024;					// 码率（4 Mbps）
		codec_ctx_en->gop_size = 1;									// GOP 大小（关键帧间隔）

		// 打开编码器
		ret = avcodec_open2(codec_ctx_en, codec_en, nullptr);
		if (ret < 0)
		{
			throw std::runtime_error("Could not open codec");
		}

		// 7. 创建输出文件上下文
		ret = avformat_alloc_output_context2(&fmt_ctx_en, nullptr, nullptr, output_filename);
		if (ret < 0)
		{
			throw std::runtime_error("Could not create output context");
		}

		// 8. 创建视频流
		stream = avformat_new_stream(fmt_ctx_en, nullptr);
		if (!stream)
		{
			throw std::runtime_error("Could not create video stream");
		}

		// 从编码器上下文复制参数到视频流
		avcodec_parameters_from_context(stream->codecpar, codec_ctx_en);
		stream->time_base = AVRational{1, 90000}; // 时间基

		// 9. 打开输出文件
		if (!(fmt_ctx_en->oformat->flags & AVFMT_NOFILE))
		{
			ret = avio_open(&fmt_ctx_en->pb, output_filename, AVIO_FLAG_WRITE);
			if (ret < 0)
			{
				throw std::runtime_error("Could not open output file");
			}
		}

		// 10. 写入文件头
		ret = avformat_write_header(fmt_ctx_en, nullptr);
		if (ret < 0)
		{
			throw std::runtime_error("Error writing header to output file");
		}
	}




//****************************************** */

	/* open the file to dump raw data */
	output_file = fopen(argv[3], "w+b");


	/* actual decoding and dump the raw data */
	while (ret >= 0)
	{
		if ((ret = av_read_frame(input_ctx, &packet)) < 0)
			break;

		if (video_stream == packet.stream_index)
			ret = decode_write(decoder_ctx, &packet); // 解码并dump文件

		av_packet_unref(&packet);
	}

	// /* flush the decoder */
	// packet.data = NULL;
	// packet.size = 0;
	// ret = decode_write(decoder_ctx, &packet);

	if(ENCODE_OPEN)
	{
		// 12. 刷新编码器（发送空帧以刷新缓冲区）
		ret = avcodec_send_frame(codec_ctx_en, nullptr);
		while (ret >= 0)
		{
			pkt = av_packet_alloc();
			ret = avcodec_receive_packet(codec_ctx_en, pkt);
			if (ret == AVERROR_EOF)
			{
				break;
			}

			// 写入剩余的数据包到输出文件
			pkt->stream_index = stream->index;
			// pkt->time_base = AVRational{1, 90000};
			ret = av_interleaved_write_frame(fmt_ctx_en, pkt);
			av_packet_unref(pkt);
		}

		// 13. 写入文件尾
		av_write_trailer(fmt_ctx_en);

		// 14. 释放资源
		if (fmt_ctx_en && !(fmt_ctx_en->oformat->flags & AVFMT_NOFILE))
		{
			avio_closep(&fmt_ctx_en->pb);
		}

		avformat_free_context(fmt_ctx_en);
		av_packet_free(&pkt);
		avcodec_free_context(&codec_ctx_en);
	}

	av_packet_unref(&packet);

	if (output_file)
		fclose(output_file);
	avcodec_free_context(&decoder_ctx);
	avformat_close_input(&input_ctx);
	av_buffer_unref(&hw_device_ctx);

	return 0;
}
