/**
 * Stream for use with emscripten in the browser. Makes use of XmlHttpRequest
 *
 * To use: git_stream_register_tls(git_open_emscripten_stream);
 *
 * If you need to access another domain, you should set the Module.jsgithost to
 * e.g. "https://somegitdomain.com" You can also add custom headers by setting
 * the Module.jsgitheaders. Example:
 *
 * Module.jsgitheaders = [{name: 'Authorization', value: 'Bearer TOKEN'}]
 *
 * Author: Peter Johan Salomonsen ( https://github.com/petersalomonsen )
 */
#ifdef __EMSCRIPTEN__
#include <emscripten.h>

extern "C" {
#include "deps/http-parser/http_parser.h"
#include "streams/stransport.h"
}

#include <string>
#include <vector>

git_stream xhrstream;

int emscripten_connect(git_stream *stream) {
  //EM_ASM(gitxhrdata = null;);
  return 1;
}

ssize_t emscripten_read(git_stream *stream, void *data, size_t len) {
  size_t ret = 0;

  //unsigned int readyState = 0;
  /*
  EM_ASM_(
    {
      if (gitxhrdata !== null) {
        // console.log("sending post data",gitxhrdata.length);
        gitxhr.send(gitxhrdata.buffer);
        gitxhrdata = null;
      }
      setValue($0, gitxhr.readyState, "i32");
    },
    &readyState);
    */

  /*
   * We skip this since we are now using a synchronous request
  while(readyState!=4) {
          EM_ASM_({
                  console.log("Waiting for data");
                  setValue($0,gitxhr.readyState,"i32");
          },&readyState);

          emscripten_sleep(10);
  }*/

  EM_ASM_(
    {
      if (gitxhr) {
        var arrayBuffer = gitxhr.response;  // Note: not oReq.responseText

        if (gitxhr.readyState === 4 && arrayBuffer) {
          var availlen = (arrayBuffer.byteLength - gitxhrreadoffset);
          var len = availlen > $2 ? $2 : availlen;

          var byteArray = new Uint8Array(arrayBuffer, gitxhrreadoffset, len);
          // console.log("read from
          // ",arrayBuffer.byteLength,gitxhrreadoffset,len,byteArray[0]);
          writeArrayToMemory(byteArray, $0);
          setValue($1, len, "i32");

          gitxhrreadoffset += len;
        }
      } else {
        setValue($1, -1, "i32");
      }
    },
    data,
    &ret,
    len);
  // printf("Returning %d bytes, requested %d\n",ret,len);
  return ret;
}

int emscripten_certificate(git_cert **out, git_stream *stream) {
  // printf("Checking certificate\n");
  return 0;
}

namespace {

  /*
      template <typename Range, typename Value = typename Range::value_type>
    std::string Join(Range const& elements, const char *const delimiter) {
        std::ostringstream os;
        auto b = begin(elements), e = end(elements);

        if (b != e) {
            std::copy(b, prev(e), std::ostream_iterator<Value>(os, delimiter));
            b = prev(e);
        }
        if (b != e) {
            os << *b;
        }

        return os.str();
    }
    */

enum class HeaderState { NONE, FIELD, VALUE };

struct XhrRequest {
  std::string method;
  std::string url;
  std::string httpVersion;
  std::string headers;
  std::string body;
  HeaderState previousState = HeaderState::NONE;
};

http_parser_settings initSettings() noexcept {
  http_parser_settings settings;
  settings.on_message_begin = [](http_parser *parser) -> int {
    XhrRequest *req = new XhrRequest;
    parser->data = req;
    return 0;
  };
  settings.on_headers_complete = [](http_parser *parser) -> int {
    XhrRequest *req = reinterpret_cast<XhrRequest *>(parser->data);
    req->method = http_method_str(static_cast<http_method>(parser->method));

    // Reserve space for the full content in advance.
    req->body.reserve(parser->content_length);
    return 0;
  };
  settings.on_url =
    [](http_parser *parser, const char *at, size_t length) -> int {
    XhrRequest *req = reinterpret_cast<XhrRequest *>(parser->data);
    req->url += std::string(at, length);
    return 0;
  };
  settings.on_header_field =
    [](http_parser *parser, const char *at, size_t length) -> int {
    XhrRequest *req = reinterpret_cast<XhrRequest *>(parser->data);
    switch (req->previousState) {
      case HeaderState::NONE:
        // First header
        req->headers = std::string(at, length);
        break;
      case HeaderState::VALUE:
        // New header.
        req->headers += "\n" + std::string(at, length);
        break;
      case HeaderState::FIELD:
        // Append to partial header field.
        req->headers += std::string(at, length);
        break;
    }
    req->previousState = HeaderState::FIELD;
    return 0;
  };
  settings.on_header_value =
    [](http_parser *parser, const char *at, size_t length) -> int {
    XhrRequest *req = reinterpret_cast<XhrRequest *>(parser->data);
    switch (req->previousState) {
      case HeaderState::NONE:
        // Error, parsed header value before field.
        return 1;
      case HeaderState::VALUE:
        // Append to partial header value.
        req->headers += std::string(at, length);
        break;
      case HeaderState::FIELD:
        // New header value
        req->headers += ":" + std::string(at, length);
        break;
    }
    req->previousState = HeaderState::VALUE;
    return 0;
  };
  settings.on_body =
    [](http_parser *parser, const char *at, size_t length) -> int {
    XhrRequest *req = reinterpret_cast<XhrRequest *>(parser->data);
    req->body += std::string(at, length);
    return 0;
  };
  settings.on_message_complete = [](http_parser *parser) -> int {
    XhrRequest *req = reinterpret_cast<XhrRequest *>(parser->data);

    /*
    printf("method: %s\n", req->method.c_str());
    printf("url: %s\n", req->url.c_str());
    printf("headers: %s\n", req->headers.c_str());
    printf("body: %s\n", req->body.c_str());
    */

  EM_ASM_(
    {
      const method = Pointer_stringify($0);
      const url = Pointer_stringify($1);
      const rawHeaders = Pointer_stringify($2);
      const body = new Uint8Array(Module.HEAPU8.buffer, $3, $4);

      const headerLines = rawHeaders.split("\n");

      const host = Module.jsgithost ? Module.jsgithost : '';
      const headers = Module.jsgitheaders ? Module.jsgitheaders : [];
      function addExtraHeaders() {
        for (var n = 0; n < headers.length; n++) {
          gitxhr.setRequestHeader(headers[n].name, headers[n].value);
        }
      }
    
      gitxhr = new XMLHttpRequest();
      gitxhrreadoffset = 0;
      gitxhr.responseType = "arraybuffer";
      // Send a synchronous request. This will run in a worker thread.
      gitxhr.open(method, host + url, false);
      for (var i = 0; i < headerLines.length; i++) {
        const splitHeader = headerLines[i].split(":", 2);
        gitxhr.setRequestHeader(splitHeader[0], splitHeader[1]);
      }
      addExtraHeaders();
      gitxhr.send(body.buffer);
    },
    req->method.c_str(),
    req->url.c_str(),
    req->headers.c_str(),
    req->body.c_str(),
    req->body.size());

  if (req) {
    delete req;
    parser->data = nullptr;
  }
  return 0;
};
return settings;
}  // namespace

http_parser* initParser() {
  http_parser *httpParser = new http_parser;
  http_parser_init(httpParser, HTTP_REQUEST);
  return httpParser;
}

// Singleton http parser and settings.
http_parser *httpParser = initParser();
http_parser_settings settings = initSettings();

}  // namespace

ssize_t emscripten_write(
  git_stream *stream, const char *data, size_t len, int flags) {

  int nparsed = http_parser_execute(httpParser, &settings, data, len);

  if (nparsed != len) {
    // Error.
    return -1;
  }
  return len;
}

int emscripten_close(git_stream *stream) { return 0; }

void emscripten_free(git_stream *stream) {
  // git__free(stream);
}

extern "C" {

int git_open_emscripten_stream(
  git_stream **out, const char *host, const char *port) {
  xhrstream.version = GIT_STREAM_VERSION;
  xhrstream.connect = emscripten_connect;
  xhrstream.read = emscripten_read;
  xhrstream.write = emscripten_write;
  xhrstream.close = emscripten_close;
  xhrstream.free = emscripten_free;
  xhrstream.certificate = emscripten_certificate;
  xhrstream.encrypted = 1;
  xhrstream.proxy_support = 0;

  *out = &xhrstream;
  return 0;
}

}  // extern "C"

#endif
