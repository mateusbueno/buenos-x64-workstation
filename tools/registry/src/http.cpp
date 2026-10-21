#include "http.hpp"

#include <cctype>
#include <charconv>

namespace velora::registry {
namespace {

ParseResult err(int status, const char* code, const char* msg) {
  ParseResult r;
  r.state = ParseState::kError;
  r.status = status;
  r.code = code;
  r.message = msg;
  return r;
}

bool token_char(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
}

}  // namespace

const std::string* HttpRequest::header(std::string_view lower_name) const {
  for (const auto& h : headers)
    if (h.first == lower_name) return &h.second;
  return nullptr;
}

ParseResult parse_http_request(std::string_view in, const HttpLimits& lim, HttpRequest& out) {
  out = HttpRequest{};
  const std::size_t end = in.find("\r\n\r\n");
  if (end == std::string_view::npos) {
    if (in.size() > lim.max_header_bytes) return err(431, "headers_too_large", "request headers exceed the size limit");
    // A bare-LF terminator is not accepted; reject early input that can not become a valid request line.
    return ParseResult{};
  }
  if (end + 4 > lim.max_header_bytes) return err(431, "headers_too_large", "request headers exceed the size limit");
  const std::string_view head = in.substr(0, end);
  for (const char c : head)
    if (c == '\0' || (static_cast<unsigned char>(c) < 0x20 && c != '\r' && c != '\n' && c != '\t')) return err(400, "bad_request", "control character in the request head");
  // Line breaks are CRLF and nothing else: a bare CR or LF (header smuggling / splitting) is refused.
  for (std::size_t i = 0; i < head.size(); ++i) {
    if (head[i] == '\r' && !(i + 1 < head.size() && head[i + 1] == '\n')) return err(400, "bad_request", "bare CR in the request head");
    if (head[i] == '\n' && !(i > 0 && head[i - 1] == '\r')) return err(400, "bad_request", "bare LF in the request head");
  }
  std::size_t line_end = head.find("\r\n");
  const std::string_view line = head.substr(0, line_end);
  // request line: METHOD SP target SP HTTP/1.1
  const std::size_t s1 = line.find(' ');
  const std::size_t s2 = s1 == std::string_view::npos ? s1 : line.find(' ', s1 + 1);
  if (s1 == std::string_view::npos || s2 == std::string_view::npos || line.find(' ', s2 + 1) != std::string_view::npos) return err(400, "bad_request", "malformed request line");
  out.method = std::string(line.substr(0, s1));
  out.target = std::string(line.substr(s1 + 1, s2 - s1 - 1));
  const std::string_view version = line.substr(s2 + 1);
  if (out.method.empty() || out.method.size() > 16) return err(400, "bad_request", "malformed request line");
  for (const char c : out.method)
    if (!(c >= 'A' && c <= 'Z')) return err(400, "bad_request", "malformed method");
  if (version != "HTTP/1.1") return err(505, "http_version_not_supported", "only HTTP/1.1 is supported");
  if (out.target.empty() || out.target[0] != '/' || out.target.size() > 2048) return err(400, "bad_request", "malformed request target");
  for (const char c : out.target)
    if (static_cast<unsigned char>(c) <= 0x20 || static_cast<unsigned char>(c) >= 0x7f) return err(400, "bad_request", "malformed request target");
  const std::size_t q = out.target.find('?');
  out.path = out.target.substr(0, q);
  if (q != std::string::npos) out.query = out.target.substr(q + 1);

  // headers
  std::size_t pos = line_end == std::string_view::npos ? head.size() : line_end + 2;
  while (pos < head.size()) {
    std::size_t e = head.find("\r\n", pos);
    if (e == std::string_view::npos) e = head.size();
    const std::string_view h = head.substr(pos, e - pos);
    pos = e + 2;
    const std::size_t colon = h.find(':');
    if (colon == std::string_view::npos || colon == 0) return err(400, "bad_request", "malformed header");
    std::string name(h.substr(0, colon));
    for (const char c : name)
      if (!token_char(c)) return err(400, "bad_request", "malformed header name");
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string_view value = h.substr(colon + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    if (out.headers.size() >= lim.max_headers) return err(431, "headers_too_large", "too many request headers");
    out.headers.emplace_back(std::move(name), std::string(value));
  }

  if (out.header("transfer-encoding")) return err(501, "unsupported_transfer_encoding", "Transfer-Encoding is not supported; send Content-Length");
  std::size_t content_length = 0;
  std::size_t n_cl = 0;
  for (const auto& h : out.headers)
    if (h.first == "content-length") {
      ++n_cl;
      const std::string& v = h.second;
      if (v.empty() || v.size() > 12 || v.find_first_not_of("0123456789") != std::string::npos) return err(400, "bad_request", "malformed Content-Length");
      std::from_chars(v.data(), v.data() + v.size(), content_length);
    }
  if (n_cl > 1) return err(400, "bad_request", "duplicate Content-Length");
  if (content_length > lim.max_body_bytes) return err(413, "body_too_large", "request body exceeds the size limit");
  const std::size_t have = in.size() - (end + 4);
  if (have > content_length) return err(400, "bad_request", "unexpected data after the request body");
  if (have < content_length) return ParseResult{};
  out.body = std::string(in.substr(end + 4, content_length));
  ParseResult ok;
  ok.state = ParseState::kComplete;
  ok.status = 200;
  ok.code = "";
  return ok;
}

const char* reason_phrase(int s) {
  switch (s) {
    case 200: return "OK";
    case 201: return "Created";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default: return "Error";
  }
}

std::string serialize_response(const HttpResponse& r) {
  std::string o = "HTTP/1.1 " + std::to_string(r.status) + " " + reason_phrase(r.status) + "\r\n";
  o += "Content-Type: application/json\r\nCache-Control: no-store\r\nConnection: close\r\n";
  for (const auto& h : r.headers) o += h.first + ": " + h.second + "\r\n";
  o += "Content-Length: " + std::to_string(r.body.size()) + "\r\n\r\n";
  o += r.body;
  return o;
}

}  // namespace velora::registry
