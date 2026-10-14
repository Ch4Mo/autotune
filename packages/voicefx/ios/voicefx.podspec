#
# VoiceFX native engine for iOS.
# CocoaPods cannot reference sources outside the pod directory, so
# Classes/ contains one-line forwarders that #include the shared sources in
# ../src (the same files Android builds through CMake).
#
Pod::Spec.new do |s|
  s.name             = 'voicefx'
  s.version          = '1.0.0'
  s.summary          = 'Offline vocal pitch-correction and effects engine.'
  s.description      = 'Pitch tracking, TD-PSOLA pitch correction, vocal chain, WAV/MP3 export.'
  s.homepage         = 'https://github.com/ch4mo/autotune'
  s.license          = { :type => 'Proprietary + LGPL (LAME)', :file => '../src/third_party/lame/COPYING' }
  s.author           = { 'VoxTune' => 'dev@example.com' }
  s.source           = { :path => '.' }
  s.source_files     = 'Classes/**/*.{c,cpp}'
  s.dependency 'Flutter'
  s.platform = :ios, '13.0'
  s.library = 'c++'
  s.pod_target_xcconfig = {
    'DEFINES_MODULE' => 'YES',
    'EXCLUDED_ARCHS[sdk=iphonesimulator*]' => 'i386',
    'CLANG_CXX_LANGUAGE_STANDARD' => 'c++17',
    'CLANG_CXX_LIBRARY' => 'libc++',
    'GCC_OPTIMIZATION_LEVEL' => '3',
    'GCC_PREPROCESSOR_DEFINITIONS' => '$(inherited) HAVE_CONFIG_H=1',
    'HEADER_SEARCH_PATHS' => '$(inherited) "${PODS_TARGET_SRCROOT}/../src" "${PODS_TARGET_SRCROOT}/../src/third_party/lame" "${PODS_TARGET_SRCROOT}/../src/third_party/lame/include" "${PODS_TARGET_SRCROOT}/../src/third_party/lame/libmp3lame"',
    'GCC_WARN_INHIBIT_ALL_WARNINGS' => 'YES',
  }
  s.swift_version = '5.0'
end
