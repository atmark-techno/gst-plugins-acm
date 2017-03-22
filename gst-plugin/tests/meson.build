gstreamer = dependency('gstreamer-1.0', version : '>1.0')
gstapp = dependency('gstreamer-app-1.0', version : '>1.0')
gstcheck = dependency('gstreamer-check-1.0', version : '>1.0')

inc = include_directories('./')

aacdec_src = ['acmaacdec.c']
aacenc_src = ['acmaacenc.c', 'utest_util.c']
h264dec_src = ['acmh264dec.c']
h264enc_src = ['acmh264enc.c', 'utest_util.c']
jpegenc_src = ['acmjpegenc.c', 'utest_util.c']
fbdevsink_src = ['acmfbdevsink.c']

test_defines = [
  '-DGST_TEST_FILES_PATH="' + meson.current_source_dir() + '/"',
 ]

h264dec = executable('acmh264dec',
                     h264dec_src,
                     c_args : test_defines,
                     dependencies : [gstreamer, gstcheck])

aacdec = executable('acmaacdec',
                    aacdec_src,
                    c_args : test_defines,
                    dependencies : [gstreamer, gstcheck])

aacenc = executable('acmaacenc',
                    aacenc_src,
                    c_args : test_defines,
                    dependencies : [gstreamer, gstcheck, gstapp],
                    include_directories : inc)

h264enc = executable('acmh264enc',
                     h264enc_src,
                     c_args : test_defines,
                     dependencies : [gstreamer, gstcheck, gstapp])

jpegenc = executable('acmjpegenc',
                     jpegenc_src,
                     c_args : test_defines,
                     dependencies : [gstreamer, gstcheck, gstapp],
                     include_directories : inc)

fbdevsink = executable('acmfbdevsink',
                       fbdevsink_src,
                       c_args : test_defines,
                       dependencies : [gstreamer, gstcheck])
