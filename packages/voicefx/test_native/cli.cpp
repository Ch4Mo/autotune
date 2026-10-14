// Command-line front-end, handy to listen to the engine on a desktop.
//   voicefx_cli input.wav output.(wav|mp3) [preset 0-7] [key 0-11|-1] [scale -1..6]
#include <cstdio>
#include <cstdlib>
#include <string>

#include "voicefx.h"

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s in.wav out.(wav|mp3) [preset] [key_root] [scale]\n", argv[0]);
    return 2;
  }
  const std::string out = argv[2];
  const int preset = argc > 3 ? std::atoi(argv[3]) : VFX_PRESET_POP;
  vfx_session* s = vfx_session_open_wav(argv[1]);
  if (!s) { std::fprintf(stderr, "error: %s\n", vfx_last_error()); return 1; }
  vfx_session_analyze(s);
  int32_t root, scale;
  float conf;
  vfx_session_detected_key(s, &root, &scale, &conf);
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  std::printf("detected key: %s %s (confidence %.2f)\n", names[root],
              scale == 1 ? "major" : scale == 2 ? "minor" : "chromatic", conf);
  vfx_params p;
  if (vfx_get_preset(preset, &p) != 0) { std::fprintf(stderr, "error: %s\n", vfx_last_error()); return 1; }
  if (argc > 4) p.key_root = std::atoi(argv[4]);
  if (argc > 5) p.scale = std::atoi(argv[5]);
  if (vfx_session_render(s, &p) != 0) { std::fprintf(stderr, "error: %s\n", vfx_last_error()); return 1; }
  const bool mp3 = out.size() > 4 && out.substr(out.size() - 4) == ".mp3";
  const int rc = mp3 ? vfx_session_export_mp3(s, out.c_str(), 320) : vfx_session_export_wav(s, out.c_str(), 24);
  if (rc != 0) { std::fprintf(stderr, "error: %s\n", vfx_last_error()); return 1; }
  std::printf("wrote %s (%.2f s)\n", out.c_str(), vfx_session_rendered_duration(s));
  vfx_session_close(s);
  return 0;
}
