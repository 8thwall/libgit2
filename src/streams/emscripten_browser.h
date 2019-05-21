#ifndef INCLUDE_streams_emscripten_browser_h__
#define INCLUDE_streams_emscripten_browser_h__

#ifdef __cplusplus
extern "C" {
#endif

extern int git_open_emscripten_stream(git_stream **out, const char *host, const char *port);

#ifdef __cplusplus
}
#endif

#endif
