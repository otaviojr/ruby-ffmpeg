#include "ruby_ffmpeg.h"
#include "ruby_ffmpeg_reader.h"
#include "ruby_ffmpeg_reader_private.h"
#include "ruby_ffmpeg_video_stream.h"
#include "ruby_ffmpeg_audio_stream.h"
#include "ruby_ffmpeg_stream.h"
#include "ruby_ffmpeg_util.h"

// Globals
static VALUE _klass;


/*
**	Object Lifetime.
*/

// Register class
VALUE reader_register_class(VALUE module, VALUE super) {
	_klass = rb_define_class_under(module, "Reader", super);
	rb_define_alloc_func(_klass, reader_alloc);

	rb_define_const (_klass, "VERSION",			rb_str_new2(human_readable_version()));
	rb_define_const (_klass, "CONFIGURATION",	rb_str_new2(avformat_configuration()));
	rb_define_const (_klass, "LICENSE",			rb_str_new2(avformat_license()));

	rb_define_method(_klass, "initialize",		reader_initialize, 1);

	rb_define_method(_klass, "name", 			reader_name, 0);
	rb_define_method(_klass, "description", 	reader_description, 0);
	rb_define_method(_klass, "start_time", 		reader_start_time, 0);
	rb_define_method(_klass, "duration", 		reader_duration, 0);
	rb_define_method(_klass, "bit_rate", 		reader_bit_rate, 0);
	rb_define_method(_klass, "streams", 		reader_streams, 0);
	rb_define_method(_klass, "metadata", 		reader_metadata, 0);

	rb_define_method(_klass, "convert", 		reader_metadata, 0);
	
	return _klass;
}

// Allocate object
VALUE reader_alloc(VALUE klass) {
	ReaderInternal * internal = (ReaderInternal *)av_mallocz(sizeof(ReaderInternal));
	if (!internal) rb_raise(rb_eNoMemError, "Failed to allocate internal structure");

	internal->format = avformat_alloc_context();
	if (!internal->format) rb_raise(rb_eNoMemError, "Failed to allocate FFMPEG format context");

	internal->protocol = avio_alloc_context(av_malloc(reader_READ_BUFFER_SIZE), reader_READ_BUFFER_SIZE, 0, internal, read_packet, NULL, NULL);
	if (!internal->protocol) rb_raise(rb_eNoMemError, "Failed to allocate FFMPEG IO context");

	internal->protocol->seekable = 0;
	internal->format->pb = internal->protocol;

	return Data_Wrap_Struct(klass, reader_mark, reader_free, (void *)internal);
}

// Free object
void reader_free(void * opaque) {
	ReaderInternal * internal = (ReaderInternal *)opaque;
	if (internal) {
		if (internal->format)
			avformat_free_context(internal->format);
		if (internal->protocol)
			av_free(internal->protocol);
		av_free(internal);
	}
}

// Mark for garbage collection
void reader_mark(void * opaque) {
	ReaderInternal * internal = (ReaderInternal *)opaque;
	if (internal) {
		rb_gc_mark(internal->io);
		rb_gc_mark(internal->streams);
		rb_gc_mark(internal->metadata);
	}
}

// Initialize object
VALUE reader_initialize(VALUE self, VALUE io) {
	ReaderInternal * internal;
	Data_Get_Struct(self, ReaderInternal, internal);

	internal->io = io;

	// Open file via Ruby stream
	int err = avformat_open_input(&internal->format, "unnamed", NULL, NULL);
	if (err) rb_raise_av_error(rb_eLoadError, err);

	// Read in stream information
	avformat_find_stream_info(internal->format, NULL);

	// Extract properties
	internal->streams = streams_to_ruby_array(self, internal->format);
	internal->metadata = av_dictionary_to_ruby_hash(internal->format->metadata);

	return self;
}


/*
**	Properties.
*/

// Name
VALUE reader_name(VALUE self) {
	ReaderInternal * internal;
	Data_Get_Struct(self, ReaderInternal, internal);

	return rb_str_new2(internal->format->iformat->name);
}

// Description
VALUE reader_description(VALUE self) {
	ReaderInternal * internal;
	Data_Get_Struct(self, ReaderInternal, internal);

	return rb_str_new2(internal->format->iformat->long_name);
}

// Start time (in seconds), nil if not available
VALUE reader_start_time(VALUE self) {
	ReaderInternal * internal;
	Data_Get_Struct(self, ReaderInternal, internal);

	return (internal->format->start_time != (int64_t)AV_NOPTS_VALUE) ? rb_float_new(internal->format->start_time / (double)AV_TIME_BASE) : Qnil;
}

// Duration (in seconds), nil if not available
VALUE reader_duration(VALUE self) {
	ReaderInternal * internal;
	Data_Get_Struct(self, ReaderInternal, internal);
	
	return (internal->format->duration != (int64_t)AV_NOPTS_VALUE) ? rb_float_new(internal->format->duration / (double)AV_TIME_BASE) : Qnil;
}

// Bit rate (in bits per second)
VALUE reader_bit_rate(VALUE self) {
	ReaderInternal * internal;
	Data_Get_Struct(self, ReaderInternal, internal);

	// We don't have a file size, and therefore not direct bit rate
	// Instead, we iterate through all streams and add them up
	int aggregate_bitrate = 0;

	unsigned i = 0;
	for(; i < internal->format->nb_streams; ++i) {
		aggregate_bitrate += internal->format->streams[i]->codec->bit_rate;
	}

	return INT2NUM(aggregate_bitrate);
}

// Media streams
VALUE reader_streams(VALUE self) {
	ReaderInternal * internal;
	Data_Get_Struct(self, ReaderInternal, internal);

	return internal->streams;
}

// Metadata
VALUE reader_metadata(VALUE self) {
	ReaderInternal * internal;
	Data_Get_Struct(self, ReaderInternal, internal);
	
	return internal->metadata;
}


/*
**	Helper Functions.
*/

// Human-readable AVFormat version
char const * human_readable_version() {
	static char version[256];
	snprintf(&version[0], sizeof(version), "%d.%d.%d", (avformat_version() >> 16) & 0xffff,
													   (avformat_version() >>  8) & 0x00ff,
													   (avformat_version()      ) & 0x00ff);
	return version;
}

// Wrap streams in Ruby objects
VALUE streams_to_ruby_array(VALUE self, AVFormatContext * format) {
	VALUE streams = rb_ary_new();

	unsigned i = 0;
	for(; i < format->nb_streams; ++i) {
		switch (format->streams[i]->codec->codec_type) {
			case AVMEDIA_TYPE_VIDEO: {
				// Video stream
				rb_ary_push(streams, video_stream_new(self, format->streams[i]));
				break;
			}
			case AVMEDIA_TYPE_AUDIO: {
				// Audio stream
				rb_ary_push(streams, audio_stream_new(self, format->streams[i]));
				break;
			}
			default: {
				// All other streams
				rb_ary_push(streams, stream_new(self, format->streams[i]));
				break;
			}
		}
	}

	return streams;
}

VALUE convert(VALUE self, VALUE dst_io)
{
	ReaderInternal * internal;
	
	Data_Get_Struct(self, ReaderInternal, internal);

	VALUE rb_filename = rb_funcall(dst_io, rb_intern("filename"), 0, NULL);
	if (TYPE(rb_filename) == T_NIL) return 0;
	
	Check_Type(rb_filename, T_STRING);
	
	char* filename = StringValue(rb_filename);	
	AVOutputFormat* dst_format = av_guess_format(NULL, filename, NULL)
	if(!dst_format) rb_raise(rb_eLoadError, "Invalid format");
	
	AVFormatContext * format_context = avformat_alloc_context();
	if(!format_context) rb_raise(rb_eNoMemError, "No enough memory - AVFormatContext");
	
	avFormatContext->oformat = dst_format;
	
	unsigned i = 0;
	for(; i < internal->format->nb_streams; ++i) {
		AVCodec* codec = NULL
		switch (internal->format->streams[i]->codec->codec_type) {
			case AVMEDIA_TYPE_VIDEO: {
				codec = avcodec_find_encoder(dst_format->video_codec);
				break;
			}
			case AVMEDIA_TYPE_AUDIO: {
				codec = avcodec_find_encoder(dst_format->audio_codec);
				break;
			}
			default: {
				continue;
			}
		}
		
		if(!codec) rb_raise(rb_eLoadError, "Encoder not found");
		
		AVStream* output_stream = avformat_new_stream(format_context,codec);
		if (!output_stream) rb_raise(rb_eNoMemError, "Error creating output stream");
		
		output_stream->codec->width = internal->format->streams[i]->codec->width;
		output_stream->codec->height = internal->format->streams[i]->codec->height;
		output_stream->codec->bit_rate = internal->format->streams[i]->codec->bit_rate;
		output_stream->codec->time_base = internal->format->streams[i]->codec->time_base;
		/* emit one intra frame every ten frames */
		output_stream->codec->gop_size = internal->format->streams[i]->codec->gop_size;
		output_stream->codec->max_b_frames = internal->format->streams[i]->codec->max_b_frames;
		output_stream->codec->pix_fmt = internal->format->streams[i]->codec->pix_fmt;
		
		if (avcodec_open2(output_stream->codec, codec, NULL) < 0) rb_raise(rb_eLoadError, "Error opening codec");
		
		AVFrame *frame = avcodec_alloc_frame();
		if(!frame) rb_raise(rb_eNoMemError, "No enough memory - AVFrame");
		
		frame->format = output_stream->codec->pix_fmt;
    	frame->width  = output_stream->codec->width;
    	frame->height = output_stream->codec->height;
    	
    	ret = av_image_alloc(frame->data, frame->linesize, frame->width, frame->height, frame->pix_fmt, 32);
    	if (ret < 0) rb_raise(rb_eNoMemError, "No enough memory - Could not allocate raw picture buffer");
    	
    	AVIOContext * protocol = avio_alloc_context(av_malloc(reader_WRITE_BUFFER_SIZE), reader_WRITE_BUFFER_SIZE, 0, &dst_io, NULL, write_packet, NULL);
    	protocol->seekable = 0;
    	format_context->pb = protocol;
    	
		// some formats want stream headers to be separate
    	if(format_context->oformat->flags & AVFMT_GLOBALHEADER)
        	output_stream->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;
        	
        avformat_write_header(format_context, NULL);
        
        //TODO: Encode
        
         av_write_trailer(avFormatContext);
         
         avcodec_close(output_stream->codec);
         av_free(output_stream->codec);
         av_free(protocol);
	}
	
	av_free(format_context);
	
	return QTrue;
}

int write_packet(void * opaque, uint8_t * buffer, int buffer_size) {
	VALUE dst_io = *((VALUE *)opaque);
	
	rb_funcall(dst_io, rb_intern("write"), rb_str_new(buffer,buffer_size));
	return buffer_size;
}

// Read next block of data
int read_packet(void * opaque, uint8_t * buffer, int buffer_size) {
	ReaderInternal * internal = (ReaderInternal *)opaque;

	VALUE string = rb_funcall(internal->io, rb_intern("read"), 1, INT2FIX(buffer_size));
	if (TYPE(string) == T_NIL) return 0;

	Check_Type(string, T_STRING);

	memcpy(buffer, RSTRING_PTR(string), RSTRING_LEN(string));
	return (int)RSTRING_LEN(string);
}

// Find the next packet for the stream
int reader_find_next_stream_packet(VALUE self, AVPacket * packet, int stream_index) {
	ReaderInternal * internal;
	Data_Get_Struct(self, ReaderInternal, internal);

	for (;;) {
		int err = av_read_frame(internal->format, packet);
		if (err < 0) {
			return 0;
		}

		if (packet->stream_index == stream_index) {
			return 1;
		}
	}
}
