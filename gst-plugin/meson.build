project('gst-plugins-acm', 'c', version : '1.0.2')

base = dependency('gstreamer-base-1.0', version : '>1.0')
audio = dependency('gstreamer-audio-1.0', version : '>1.0')
video = dependency('gstreamer-video-1.0', version : '>1.0')
pbutils = dependency('gstreamer-pbutils-1.0', version : '>1.0')
codecparser = dependency('gstreamer-codecparsers-1.0', version : '>1.0')

inc = include_directories('include')

v4l2_src = ['src/gstacmv4l2_util.c',
              'src/gstacmdmabufmeta.c',
              'src/gstacmv4l2bufferpool.c']
debug_src = ['src/gstacm_debug.c']

aacdec_src = ['src/gstacmaacdec.c']
aacenc_src = ['src/gstacmaacenc.c']
h264dec_src = ['src/gstacmh264dec.c']
h264enc_src = ['src/gstacmh264enc.c']
jpegenc_src = ['src/gstacmjpegenc.c']
fbdevsink_src = ['src/gstacmfbdevsink.c']

cdata = configuration_data()
cdata.set_quoted('PACKAGE', meson.project_name())
cdata.set_quoted('VERSION', meson.project_version())
configure_file(output : 'config.h', configuration : cdata)

add_global_arguments('-DHAVE_CONFIG_H', language : 'c')

v4l2 = shared_library('gstacmv4l2',
                      v4l2_src,
                      dependencies : [base],
                      include_directories : inc)

h264dec = library('gstacmh264dec',
                  h264dec_src,
                  dependencies : [video, codecparser],
                  link_with : v4l2)

aacdec = library('gstacmaacdec',
                 aacdec_src,
                 dependencies : [audio],
                 link_with : v4l2)

aacenc = library('gstacmaacenc',
                 aacenc_src,
                 dependencies : [audio],
                 include_directories : inc,
                 link_with : v4l2)

h264enc = library('gstacmh264enc',
                  h264enc_src,
                  dependencies : [video, pbutils],
                  include_directories : inc,
                  link_with : v4l2)

jpegenc = library('gstacmjpegenc',
                  jpegenc_src,
                  dependencies : [video],
                  include_directories : inc,
                  link_with : v4l2)

fbdevsink = library('gstacmfbdevsink',
                    fbdevsink_src,
                    dependencies : [video],
                    include_directories : inc,
                    link_with : v4l2)

subdir('tests')
