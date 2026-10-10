project_root = "../../.."
include(project_root.."/tools/build")

group("src")
project("xenia-apu")
  uuid("f4df01f0-50e4-4c67-8f54-61660696cc79")
  kind("StaticLib")
  language("C++")
  links({
    "libavcodec",
    "libavutil",
    "xenia-base",
  })
  defines({
  })
  includedirs({
    project_root.."/third_party/FFmpeg/",
    project_root.."/third_party/rapidjson/include/",
  })
  local_platform_files()
  -- Own subdirectories (unlike sdl/, xaudio2/, nop/, which are separate
  -- sibling projects with their own premake5.lua and backend-specific
  -- include paths).
  local_platform_files("music_assistant")
  local_platform_files("sendspin")
