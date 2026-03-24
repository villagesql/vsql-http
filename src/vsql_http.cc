// Copyright (c) 2026 VillageSQL Contributors
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2.0,
// as published by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License, version 2.0, for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

// vsql_http — HTTP client extension for VillageSQL
//
// pgsql-http-compatible functions returning JSON responses:
//   {"status": <int>, "content_type": "<str>",
//    "headers": [["name","value"],...], "content": "<str>"}
//
// All HTTP functions return NULL on curl-level failure (connection refused,
// DNS failure, timeout). Use JSON_VALUE(result, '$.status') to inspect the
// HTTP status code.

#include <villagesql/extension.h>

#include <curl/curl.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

using namespace villagesql::extension_builder;

// ============================================================
// curl global init (process-wide, once)
// ============================================================

static std::once_flag g_curl_once;

static void ensure_curl_init() {
  std::call_once(g_curl_once,
                 []() { curl_global_init(CURL_GLOBAL_ALL); });
}

// Return a thread-local curl handle, creating it on first use per thread.
// The handle is reused across calls (connection keep-alive). curl_easy_reset()
// clears options between requests without closing the connection pool.
static CURL *get_curl_handle() {
  ensure_curl_init();
  thread_local struct Holder {
    CURL *h;
    Holder() : h(curl_easy_init()) {}
    ~Holder() { if (h) curl_easy_cleanup(h); }
  } holder;
  return holder.h;
}

// ============================================================
// Internal helpers
// ============================================================

// Accumulate response body via curl write callback.
static size_t write_callback(char *data, size_t size, size_t nmemb,
                              void *userp) {
  static_cast<std::string *>(userp)->append(data, size * nmemb);
  return size * nmemb;
}

// Append additional slist to list in-place. Handles the null-list case.
static void append_slist(struct curl_slist **list,
                         struct curl_slist *additional) {
  if (!additional) return;
  if (!*list) { *list = additional; return; }
  struct curl_slist *tail = *list;
  while (tail->next) tail = tail->next;
  tail->next = additional;
}

// JSON-escape a string for embedding as a JSON string value (RFC 8259).
static std::string json_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() * 2);  // worst case: every char expands to 2+ bytes
  for (unsigned char c : s) {
    if (c == '"') {
      out += "\\\"";
    } else if (c == '\\') {
      out += "\\\\";
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else if (c < 0x20 || c == 0x7f) {
      char buf[8];
      snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
      out += buf;
    } else {
      out += static_cast<char>(c);
    }
  }
  return out;
}

// Collect response headers as JSON array-of-pairs content.
// Called by curl once per header line (including status line and blank separator).
// Appends ["name", "value"] entries to a std::string accumulator (no outer
// brackets — caller wraps in [...] to form the JSON array).
// Header names are lowercased per HTTP/2 convention, matching pgsql-http.
static size_t header_callback(char *data, size_t size, size_t nmemb,
                               void *userp) {
  size_t len = size * nmemb;
  std::string_view line(data, len);
  // Trim trailing CRLF.
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
    line.remove_suffix(1);
  // Skip HTTP status line (e.g. "HTTP/1.1 200 OK") and blank separator.
  if (line.empty() || (line.size() >= 5 && line.substr(0, 5) == "HTTP/"))
    return len;
  // Split on ": " — fall back to ":" for headers without a space after colon.
  std::string_view name, value;
  size_t colon = line.find(": ");
  if (colon != std::string_view::npos) {
    name = line.substr(0, colon);
    value = line.substr(colon + 2);
  } else {
    colon = line.find(':');
    if (colon == std::string_view::npos) return len;
    name = line.substr(0, colon);
    value = (colon + 1 < line.size()) ? line.substr(colon + 1) : "";
  }
  // Lowercase the header name.
  std::string name_lc(name);
  for (char &c : name_lc)
    if (c >= 'A' && c <= 'Z') c += 32;
  auto *out = static_cast<std::string *>(userp);
  if (!out->empty()) *out += ',';
  *out += "[\"";
  *out += json_escape(name_lc);
  *out += "\",\"";
  *out += json_escape(value);
  *out += "\"]";
  return len;
}

// Parse a JSON object {"key": "value", ...} into a curl_slist of
// "Key: Value" header strings. Returns nullptr on empty/invalid input.
// Caller must free the result with curl_slist_free_all.
static struct curl_slist *parse_json_headers(std::string_view json) {
  if (json.empty()) return nullptr;

  struct curl_slist *list = nullptr;
  size_t i = 0;
  const size_t n = json.size();

  auto skip_ws = [&]() {
    while (i < n && (json[i] == ' ' || json[i] == '\t' ||
                     json[i] == '\n' || json[i] == '\r'))
      ++i;
  };

  // Advance past opening '"', parse content, advance past closing '"'.
  auto parse_str = [&]() -> std::string {
    if (i >= n || json[i] != '"') return {};
    ++i;
    std::string r;
    r.reserve(32);
    while (i < n && json[i] != '"') {
      if (json[i] == '\\' && i + 1 < n) {
        ++i;
        switch (json[i]) {
          case '"':  r += '"';  break;
          case '\\': r += '\\'; break;
          case '/':  r += '/';  break;
          case 'n':  r += '\n'; break;
          case 'r':  r += '\r'; break;
          case 't':  r += '\t'; break;
          default:   r += json[i]; break;
        }
      } else {
        r += json[i];
      }
      ++i;
    }
    if (i < n) ++i;  // skip closing '"'
    return r;
  };

  skip_ws();
  if (i >= n || json[i] != '{') return nullptr;
  ++i;

  while (i < n) {
    skip_ws();
    if (i >= n || json[i] == '}') break;
    if (json[i] == ',') { ++i; continue; }
    std::string key = parse_str();
    skip_ws();
    if (i >= n || json[i] != ':') break;
    ++i;
    skip_ws();
    std::string val = parse_str();
    if (!key.empty()) {
      std::string header = key + ": " + val;
      list = curl_slist_append(list, header.c_str());
    }
  }
  return list;
}

// ============================================================
// Per-call HTTP options
// ============================================================

struct HttpOptions {
  long timeout_s = 30;
  std::string proxy;
  std::string user_agent;
  std::string ssl_cert;
  std::string ssl_key;
  std::string ssl_ca_bundle;
};

// Parse an options JSON object into HttpOptions.
// Supported keys: "timeout" (integer seconds), "proxy", "user_agent",
//                 "ssl_cert", "ssl_key", "ssl_ca_bundle" (all strings).
static HttpOptions parse_options(std::string_view json) {
  HttpOptions opts;
  if (json.empty()) return opts;

  size_t i = 0;
  const size_t n = json.size();

  auto skip_ws = [&]() {
    while (i < n && (json[i] == ' ' || json[i] == '\t' ||
                     json[i] == '\n' || json[i] == '\r'))
      ++i;
  };

  auto parse_str = [&]() -> std::string {
    if (i >= n || json[i] != '"') return {};
    ++i;
    std::string r;
    while (i < n && json[i] != '"') {
      if (json[i] == '\\' && i + 1 < n) { ++i; r += json[i]; }
      else r += json[i];
      ++i;
    }
    if (i < n) ++i;
    return r;
  };

  auto parse_long = [&]() -> long {
    bool neg = (i < n && json[i] == '-');
    if (neg) ++i;
    long v = 0;
    while (i < n && json[i] >= '0' && json[i] <= '9')
      v = v * 10 + (json[i++] - '0');
    return neg ? -v : v;
  };

  skip_ws();
  if (i >= n || json[i] != '{') return opts;
  ++i;

  while (i < n) {
    skip_ws();
    if (i >= n || json[i] == '}') break;
    if (json[i] == ',') { ++i; continue; }
    std::string key = parse_str();
    skip_ws();
    if (i >= n || json[i] != ':') break;
    ++i;
    skip_ws();
    if (key == "timeout") {
      opts.timeout_s = parse_long();
    } else if (i < n && json[i] == '"') {
      std::string val = parse_str();
      if (key == "proxy")          opts.proxy        = std::move(val);
      else if (key == "user_agent")     opts.user_agent   = std::move(val);
      else if (key == "ssl_cert")       opts.ssl_cert     = std::move(val);
      else if (key == "ssl_key")        opts.ssl_key      = std::move(val);
      else if (key == "ssl_ca_bundle")  opts.ssl_ca_bundle = std::move(val);
    } else {
      // Skip unknown value.
      while (i < n && json[i] != ',' && json[i] != '}') ++i;
    }
  }
  return opts;
}

// ============================================================
// Core HTTP executor
// ============================================================

// Returns JSON response string, or empty string on curl-level failure.
// Response shape: {"status": N, "content_type": "...", "headers": [...],
//                  "content": "..."}
static std::string do_http(std::string_view method, std::string_view url,
                            std::string_view headers_json,
                            std::string_view body,
                            std::string_view content_type,
                            const HttpOptions &opts = HttpOptions{}) {
  CURL *curl = get_curl_handle();
  if (!curl) return {};
  curl_easy_reset(curl);  // clear options from previous call; keeps connection pool

  std::string response_body;
  std::string response_headers;  // array-of-pairs content (without brackets)
  // Convert string_views to owned strings: curl requires null-terminated
  // C strings and stable pointers for the duration of curl_easy_perform.
  std::string url_str(url);
  std::string body_str(body);
  std::string method_str(method);
  struct curl_slist *hdrs = nullptr;

  // RAII: clean up header list on all exit paths (handle is thread-local).
  struct Guard {
    struct curl_slist **h;
    ~Guard() { if (*h) curl_slist_free_all(*h); }
  } guard{&hdrs};

  curl_easy_setopt(curl, CURLOPT_URL, url_str.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, opts.timeout_s);
  if (!opts.proxy.empty())
    curl_easy_setopt(curl, CURLOPT_PROXY, opts.proxy.c_str());
  if (!opts.user_agent.empty())
    curl_easy_setopt(curl, CURLOPT_USERAGENT, opts.user_agent.c_str());
  if (!opts.ssl_cert.empty())
    curl_easy_setopt(curl, CURLOPT_SSLCERT, opts.ssl_cert.c_str());
  if (!opts.ssl_key.empty())
    curl_easy_setopt(curl, CURLOPT_SSLKEY, opts.ssl_key.c_str());
  if (!opts.ssl_ca_bundle.empty())
    curl_easy_setopt(curl, CURLOPT_CAINFO, opts.ssl_ca_bundle.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_headers);

  // Set HTTP method and body.
  if (method_str == "GET") {
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  } else if (method_str == "POST") {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(body_str.size()));
  } else {
    // PUT, DELETE, PATCH, HEAD, OPTIONS, or any custom method.
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method_str.c_str());
    if (!body_str.empty()) {
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                       static_cast<long>(body_str.size()));
    }
  }

  // Build header list: Content-Type first, then caller-supplied headers.
  if (!content_type.empty()) {
    std::string ct_hdr;
    ct_hdr.reserve(14 + content_type.size());
    ct_hdr += "Content-Type: ";
    ct_hdr += content_type;
    hdrs = curl_slist_append(hdrs, ct_hdr.c_str());
  }
  append_slist(&hdrs, parse_json_headers(headers_json));
  if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

  CURLcode res = curl_easy_perform(curl);
  if (res != CURLE_OK) return {};

  long status_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);

  char *ct_ptr = nullptr;
  curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct_ptr);
  std::string_view resp_ct = ct_ptr ? ct_ptr : "";

  // Build JSON response.
  std::string out;
  out.reserve(128 + response_headers.size() + response_body.size() * 2);
  out += "{\"status\": ";
  out += std::to_string(status_code);
  out += ", \"content_type\": \"";
  out += json_escape(resp_ct);
  out += "\", \"headers\": [";
  out += response_headers;
  out += "], \"content\": \"";
  out += json_escape(response_body);
  out += "\"}";
  return out;
}

// ============================================================
// Result output
//
// The VEF provides a caller-managed str_buf of size max_str_len.
// Write into it with a bounds check. Responses larger than max_str_len are
// truncated — buffer_size() on each function registration controls the
// pre-allocated size. HTTP functions use 256KB; urlencode/urldecode use 8KB.
// ============================================================

static inline void set_result(vef_vdf_result_t *result, const std::string &s) {
  size_t copy_len = s.size();
  if (copy_len > result->max_str_len) copy_len = result->max_str_len;
  memcpy(result->str_buf, s.data(), copy_len);
  result->actual_len = copy_len;
  result->type = VEF_RESULT_VALUE;
}

// Helpers to convert vef_invalue_t to string_view.
#define SV(arg) std::string_view{(arg)->str_value, (arg)->str_len}
#define SV_OR_EMPTY(arg) ((arg)->is_null ? std::string_view{} : SV(arg))

// ============================================================
// VDF implementations
// ============================================================

// Shared wrapper: null-checks url, calls fn() to get the result string,
// handles NULL/empty/error outcomes, and writes to the result buffer.
template <typename Fn>
static void http_call(vef_invalue_t *url, vef_vdf_result_t *result,
                      const char *func_name, Fn fn) {
  try {
    if (url->is_null) { result->type = VEF_RESULT_NULL; return; }
    std::string r = fn();
    if (r.empty()) { result->type = VEF_RESULT_NULL; return; }
    set_result(result, r);
  } catch (...) {
    result->type = VEF_RESULT_ERROR;
    snprintf(result->error_msg, VEF_MAX_ERROR_LEN, "%s: internal error",
             func_name);
  }
}

static void http_get_1_impl(vef_context_t *, vef_invalue_t *url,
                             vef_vdf_result_t *result) {
  http_call(url, result, "http_get",
            [&] { return do_http("GET", SV(url), {}, {}, {}); });
}

static void http_post_3_impl(vef_context_t *, vef_invalue_t *url,
                              vef_invalue_t *ct, vef_invalue_t *body,
                              vef_vdf_result_t *result) {
  http_call(url, result, "http_post", [&] {
    return do_http("POST", SV(url), {}, SV_OR_EMPTY(body), SV_OR_EMPTY(ct));
  });
}

static void http_put_3_impl(vef_context_t *, vef_invalue_t *url,
                             vef_invalue_t *ct, vef_invalue_t *body,
                             vef_vdf_result_t *result) {
  http_call(url, result, "http_put", [&] {
    return do_http("PUT", SV(url), {}, SV_OR_EMPTY(body), SV_OR_EMPTY(ct));
  });
}

static void http_delete_1_impl(vef_context_t *, vef_invalue_t *url,
                                vef_vdf_result_t *result) {
  http_call(url, result, "http_delete",
            [&] { return do_http("DELETE", SV(url), {}, {}, {}); });
}

static void http_patch_3_impl(vef_context_t *, vef_invalue_t *url,
                               vef_invalue_t *ct, vef_invalue_t *body,
                               vef_vdf_result_t *result) {
  http_call(url, result, "http_patch", [&] {
    return do_http("PATCH", SV(url), {}, SV_OR_EMPTY(body), SV_OR_EMPTY(ct));
  });
}

// http(method, url, headers_json, body, content_type, options_json)
// options_json: NULL or JSON object with keys: timeout (int seconds),
//   proxy, user_agent, ssl_cert, ssl_key, ssl_ca_bundle (all strings).
// Example: '{"timeout": 5, "proxy": "http://proxy:8080"}'
static void http_6_impl(vef_context_t *, vef_invalue_t *method,
                         vef_invalue_t *url, vef_invalue_t *hdrs,
                         vef_invalue_t *body, vef_invalue_t *ct,
                         vef_invalue_t *options,
                         vef_vdf_result_t *result) {
  http_call(url, result, "http_request", [&] {
    std::string_view m = method->is_null ? std::string_view{"GET"} : SV(method);
    HttpOptions opts = options->is_null ? HttpOptions{} :
                                          parse_options(SV(options));
    return do_http(m, SV(url), SV_OR_EMPTY(hdrs), SV_OR_EMPTY(body),
                   SV_OR_EMPTY(ct), opts);
  });
}

// Shared wrapper for urlencode/urldecode. Op must return {char*, size_t} where
// the char* was allocated by curl (freed here via curl_free) and size_t is the
// byte count. Returns NULL to the caller on curl init failure or op failure.
template <typename Op>
static void curl_codec_impl(vef_invalue_t *input, vef_vdf_result_t *result,
                             const char *func_name, Op op) {
  try {
    if (input->is_null) { result->type = VEF_RESULT_NULL; return; }
    CURL *curl = get_curl_handle();
    if (!curl) { result->type = VEF_RESULT_NULL; return; }
    auto [buf, buf_len] = op(curl);
    if (!buf) { result->type = VEF_RESULT_NULL; return; }
    size_t copy_len = buf_len < result->max_str_len ? buf_len : result->max_str_len;
    memcpy(result->str_buf, buf, copy_len);
    curl_free(buf);
    result->actual_len = copy_len;
    result->type = VEF_RESULT_VALUE;
  } catch (...) {
    result->type = VEF_RESULT_ERROR;
    snprintf(result->error_msg, VEF_MAX_ERROR_LEN, "%s: internal error", func_name);
  }
}

// urlencode(text) — percent-encode a string using curl_easy_escape.
static void urlencode_impl(vef_context_t *, vef_invalue_t *input,
                            vef_vdf_result_t *result) {
  curl_codec_impl(input, result, "urlencode", [&](CURL *c) {
    char *r = curl_easy_escape(c, input->str_value,
                               static_cast<int>(input->str_len));
    return std::pair<char *, size_t>{r, r ? strlen(r) : 0};
  });
}

// urldecode(text) — decode a percent-encoded string using curl_easy_unescape.
static void urldecode_impl(vef_context_t *, vef_invalue_t *input,
                            vef_vdf_result_t *result) {
  curl_codec_impl(input, result, "urldecode", [&](CURL *c) {
    int n = 0;
    char *r = curl_easy_unescape(c, input->str_value,
                                 static_cast<int>(input->str_len), &n);
    return std::pair<char *, size_t>{r, static_cast<size_t>(n)};
  });
}

#undef SV
#undef SV_OR_EMPTY

// ============================================================
// Registration
// ============================================================

// Buffer sizes:
//   HTTP functions: 256KB — covers typical API responses used in SQL queries.
//   urlencode/urldecode: 8KB — URL-encoded strings are rarely larger.
// Responses exceeding the buffer are truncated at max_str_len bytes.
static constexpr size_t kHttpBufSize = 256 * 1024;
static constexpr size_t kEncodeBufSize = 8 * 1024;

// Note: VEF does not support function overloading. Each function name maps
// to exactly one signature. For HTTP calls with custom headers, use http().
VEF_GENERATE_ENTRY_POINTS(
    make_extension("vsql_http", "0.0.1")
        .func(make_func<&http_get_1_impl>("http_get")
                  .returns(STRING).param(STRING)
                  .buffer_size(kHttpBufSize).build())
        .func(make_func<&http_post_3_impl>("http_post")
                  .returns(STRING).param(STRING).param(STRING).param(STRING)
                  .buffer_size(kHttpBufSize).build())
        .func(make_func<&http_put_3_impl>("http_put")
                  .returns(STRING).param(STRING).param(STRING).param(STRING)
                  .buffer_size(kHttpBufSize).build())
        .func(make_func<&http_delete_1_impl>("http_delete")
                  .returns(STRING).param(STRING)
                  .buffer_size(kHttpBufSize).build())
        .func(make_func<&http_patch_3_impl>("http_patch")
                  .returns(STRING).param(STRING).param(STRING).param(STRING)
                  .buffer_size(kHttpBufSize).build())
        .func(make_func<&http_6_impl>("http_request")
                  .returns(STRING).param(STRING).param(STRING).param(STRING)
                  .param(STRING).param(STRING).param(STRING)
                  .buffer_size(kHttpBufSize).build())
        .func(make_func<&urlencode_impl>("url_encode")
                  .returns(STRING).param(STRING)
                  .buffer_size(kEncodeBufSize).build())
        .func(make_func<&urldecode_impl>("url_decode")
                  .returns(STRING).param(STRING)
                  .buffer_size(kEncodeBufSize).build()))
