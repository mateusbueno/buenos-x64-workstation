#pragma once
// Story 6.8: the minimal HTTP/1.1 the registry speaks (one request per connection, Connection: close, Content-Length bodies only). Pure functions, no I/O,
// so the parser can be fuzzed directly.
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace velora::registry {

struct HttpLimits {
  std::size_t max_header_bytes = 8192;   // request line + headers + the blank line
  std::size_t max_body_bytes = 65536;
  std::size_t max_headers = 40;
};

struct HttpRequest {
  std::string method;                                       // upper case token
  std::string target;                                       // as sent
  std::string path;                                         // target without the query
  std::string query;                                        // after '?', not decoded
  std::vector<std::pair<std::string, std::string>> headers; // names lower-cased
  std::string body;
  const std::string* header(std::string_view lower_name) const;
};

struct HttpResponse {
  int status = 200;
  std::string body;                                         // JSON
  std::vector<std::pair<std::string, std::string>> headers;  // extra headers (Retry-After)
};

enum class ParseState { kNeedMore, kComplete, kError };
struct ParseResult {
  ParseState state = ParseState::kNeedMore;
  int status = 400;                 // HTTP status to answer with when kError
  const char* code = "bad_request"; // typed error code
  const char* message = "";
};

// Parses what has been received so far. kComplete when the header block and Content-Length body are all present (trailing bytes are an error: one request per connection).
ParseResult parse_http_request(std::string_view received, const HttpLimits& limits, HttpRequest& out);

const char* reason_phrase(int status);
std::string serialize_response(const HttpResponse& r);

}  // namespace velora::registry
